#pragma once

#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/associated_allocator.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>

#include "ruvia/core/detail/suspend_race_state.h"
#include "ruvia/core/task.h"

namespace ruvia::detail {

template <typename t_type>
using task_completion_result =
    std::variant<std::conditional_t<std::is_void_v<t_type>, std::monostate, t_type>, std::exception_ptr>;
template <typename t_type, typename handler_type>
class task_completion_state final {
public:
    using handler_allocator_type = asio::associated_allocator_t<handler_type>;
    using state_allocator_type = typename std::allocator_traits<handler_allocator_type>::template rebind_alloc<
        task_completion_state>;
    using state_allocator_traits_type = std::allocator_traits<state_allocator_type>;

    static task_completion_state* create(task<t_type>&& task_value, handler_type&& handler_value) {
        handler_allocator_type handler_allocator(asio::get_associated_allocator(handler_value));
        state_allocator_type state_allocator(handler_allocator);
        auto* state_value = state_allocator_traits_type::allocate(state_allocator, 1);
        try {
            state_allocator_traits_type::construct(state_allocator, state_value, std::move(task_value),
                std::forward<handler_type>(handler_value), std::move(handler_allocator));
        } catch (...) {
            state_allocator_traits_type::deallocate(state_allocator, state_value, 1);
            throw;
        }
        return state_value;
    }

    task_completion_state(
        task<t_type>&& task_value, handler_type&& handler_value, handler_allocator_type allocator_value)
        : task_(std::move(task_value)),
          handler_(std::forward<handler_type>(handler_value)),
          allocator_(std::move(allocator_value)) {}

    void start() {
        task_.handle_.promise().control_.set_completion(this, &task_completion_state::complete);
        task_.start();
    }

    static void complete(void* raw) noexcept {
        auto* state_value = static_cast<task_completion_state*>(raw);
        auto executor = asio::get_associated_executor(state_value->handler_);
        auto completion_allocator = state_value->allocator_;
        state_owner_type owner_value(state_value, state_deleter_type{state_allocator_type(state_value->allocator_)});
        asio::post(executor, asio::bind_allocator(std::move(completion_allocator),
                                 [owner_value = std::move(owner_value)]() mutable {
                                     deliver(std::move(owner_value));
                                 }));
    }

private:
    struct state_deleter_type final {
        void operator()(task_completion_state* state_value) noexcept {
            state_allocator_traits_type::destroy(allocator_, state_value);
            state_allocator_traits_type::deallocate(allocator_, state_value, 1);
        }

        state_allocator_type allocator_;
    };

    using state_owner_type = std::unique_ptr<task_completion_state, state_deleter_type>;

    static void deliver(state_owner_type owner_value) {
        // The delivery owner outlives the result and every callback argument.
        // An owning completion handler can publish external readiness in its
        // destructor only after moved-from result storage has been reclaimed.
        auto handler = std::move(owner_value->handler_);
        owner_value->task_.prepare_direct_completion();
        auto result_value = [&owner_value]() -> task_completion_result<t_type> {
            try {
                if constexpr (std::is_void_v<t_type>) {
                    owner_value->task_.handle_.promise().result();
                    return task_completion_result<t_type>(std::in_place_index<0>, std::monostate{});
                } else {
                    return task_completion_result<t_type>(
                        std::in_place_index<0>, owner_value->task_.handle_.promise().result());
                }
            } catch (...) {
                return task_completion_result<t_type>(std::in_place_index<1>, std::current_exception());
            }
        }();
        owner_value->task_.finish_direct_completion();
        owner_value.reset();
        std::move(handler)(std::move(result_value));
    }

    task<t_type> task_;
    handler_type handler_;
    handler_allocator_type allocator_;
};

template <typename t_type, typename completion_token_type>
    requires asio_task_result<t_type>
inline auto async_start_task(task<t_type>&& pending_task, completion_token_type&& token) {
    pending_task.prepare_direct_start();
    // MSVC cannot pass over-aligned results by value. Delivery owns the result
    // until the handler returns, so its completion argument can be an rvalue reference.
    return asio::async_initiate<completion_token_type, void(task_completion_result<t_type>&&)>(
        [](auto&& handler, task<t_type> task_value) {
            using handler_type = std::decay_t<decltype(handler)>;
            auto* state_value = task_completion_state<t_type, handler_type>::create(
                std::move(task_value), std::forward<decltype(handler)>(handler));
            state_value->start();
        },
        token, std::move(pending_task));
}

template <typename result_type, typename initiate_type>
class asio_completion_awaiter;
template <typename result_type>
class asio_completion final {
public:
    [[nodiscard]] static asio_completion completed(std::error_code error_code, result_type result_value) {
        return asio_completion(error_code, std::move(result_value));
    }

    [[nodiscard]] std::error_code error_code() const noexcept {
        return error_code_;
    }

    [[nodiscard]] const result_type& result() const& noexcept {
        return result_;
    }
    const result_type& result() const&& = delete;

    [[nodiscard]] result_type& result() & noexcept {
        return result_;
    }
    result_type& result() && = delete;

    [[nodiscard]] result_type take_result() && {
        return std::move(result_);
    }

private:
    template <typename, typename>
    friend class asio_completion_awaiter;

    asio_completion(std::error_code error_code, result_type&& result_value) noexcept(
        std::is_nothrow_move_constructible_v<result_type>)
        : error_code_(error_code),
          result_(std::move(result_value)) {}

    std::error_code error_code_;
    result_type result_;
};

template <>
class asio_completion<void> final {
public:
    [[nodiscard]] static asio_completion completed(std::error_code error_code) noexcept {
        return asio_completion(error_code);
    }

    [[nodiscard]] std::error_code error_code() const noexcept {
        return error_code_;
    }

private:
    template <typename, typename>
    friend class asio_completion_awaiter;

    explicit asio_completion(std::error_code error_code) noexcept
        : error_code_(error_code) {}

    std::error_code error_code_;
};

// Asio completion signatures always provide an error code and may also provide
// a result that remains meaningful on partial failure (for example transferred
// bytes). The completion therefore owns both fields; only pending versus
// completed is exclusive.
template <typename result_type, typename initiate_type>
class asio_completion_awaiter final {
public:
    explicit asio_completion_awaiter(initiate_type initiate)
        : initiate_(std::move(initiate)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    // If initiate_ throws, the exception propagates from the await-expression
    // directly ([expr.await]/5), without an exception_ptr side channel.
    //
    // An asio initiation may complete synchronously (for example a closed
    // socket or a resolver cache hit), invoking the completion handler before
    // await_suspend returns. Resuming the continuation from inside that window
    // would run the coroutine while it is still suspended-by-await_suspend and
    // potentially destroy its frame before this function returns. The
    // suspend_race_state records the completion instead; the return value of
    // await_suspend then reports the actual race order, so a synchronous
    // completion is consumed by await_resume without resuming at all.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        if constexpr (std::is_void_v<result_type>) {
            initiate_([this, handle](std::error_code ec, auto&&...) mutable {
                if (state_.complete(asio_completion<void>::completed(ec))) {
                    handle.resume();
                }
            });
        } else {
            initiate_([this, handle](std::error_code ec, result_type result_value) mutable {
                if (state_.complete(asio_completion<result_type>::completed(ec, std::move(result_value)))) {
                    handle.resume();
                }
            });
        }
        return state_.suspend(handle);
    }

    [[nodiscard]] asio_completion<result_type> await_resume() {
        return state_.take_value();
    }

private:
    initiate_type initiate_;
    suspend_race_state<asio_completion<result_type>> state_;
};

}  // namespace ruvia::detail
