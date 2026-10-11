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
    if (const auto* failure = result_value.failure()) {
        std::rethrow_exception(failure->exception());
    }
    if constexpr (std::is_void_v<t_type>) {
        co_return;
    } else {
        co_return std::move(*result_value.success()).take_value();
    }
}

}  // namespace ruvia
