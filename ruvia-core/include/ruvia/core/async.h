#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/task.h"

namespace ruvia {

// Adapts a lazy task for an Asio operation owner such as co_spawn. The returned
// awaitable retains the task until completion; the caller still owns the
// completion token and must wait for it before tearing down the executor.
template <typename t_type>
    requires detail::asio_task_result<t_type>
[[nodiscard]] asio::awaitable<t_type> as_awaitable(task<t_type> task_value) {
    auto result_value = co_await detail::async_start_task(std::move(task_value), asio::use_awaitable);
    if (result_value.index() == 1) {
        std::rethrow_exception(std::get<1>(result_value));
    }
    if constexpr (std::is_void_v<t_type>) {
        co_return;
    } else {
        co_return std::move(std::get<0>(result_value));
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
