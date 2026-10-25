#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/task.h"

namespace ruvia {

namespace detail {

template <typename t_type>
    requires asio_task_result<t_type> && (!std::is_void_v<t_type>)
[[nodiscard]] asio::awaitable<t_type> await_non_void_task(task<t_type> task_value) {
    auto result_value = co_await async_start_task(std::move(task_value), asio::use_awaitable);
    if (result_value.index() == 1) {
        std::rethrow_exception(std::get<1>(result_value));
    }
    co_return std::move(std::get<0>(result_value));
}

}  // namespace detail

// Adapts a lazy task for an Asio operation owner such as co_spawn. Void tasks
// complete with std::exception_ptr (an Asio disposition), so use_awaitable
// produces the awaitable<void> directly without a wrapper coroutine frame.
template <typename t_type>
    requires detail::asio_task_result<t_type>
[[nodiscard]] asio::awaitable<t_type> as_awaitable(task<t_type> task_value) {
    if constexpr (std::is_void_v<t_type>) {
        return detail::async_start_task(std::move(task_value), asio::use_awaitable);
    } else {
        return detail::await_non_void_task(std::move(task_value));
    }
}

// Adapt a completion-based Asio operation for use with a Ruvia coroutine.
// The awaiter owns the completion state until the operation finishes.
template <typename result_type = void, typename initiate_type>
[[nodiscard]] auto async_asio(initiate_type&& initiate) {
    return detail::asio_completion_awaiter<result_type, std::decay_t<initiate_type>>(
        std::forward<initiate_type>(initiate));
}

}  // namespace ruvia
