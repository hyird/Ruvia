#pragma once

#include <utility>

#include <asio/awaitable.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/task.h"

namespace ruvia {

// Adapts a lazy task for an Asio operation owner such as co_spawn. The returned
// awaitable retains the task until completion; the caller still owns the
// completion token and must wait for it before tearing down the executor.
template <typename t_type>
    requires detail::asio_task_result<t_type>
[[nodiscard]] asio::awaitable<t_type> as_awaitable(task<t_type> task_value) {
    return detail::task_as_awaitable(std::move(task_value));
}

}  // namespace ruvia
