#pragma once

#include <utility>

#include <asio/awaitable.hpp>

#include <ruvia/core/Task.h>
#include <ruvia/core/detail/io/AsioAwait.h>

namespace ruvia {

// Adapts a lazy Task for an Asio operation owner such as co_spawn. The returned
// awaitable retains the Task until completion; the caller still owns the
// completion token and must wait for it before tearing down the executor.
template <typename T>
    requires detail::AsioTaskResult<T>
[[nodiscard]] asio::awaitable<T> asAwaitable(Task<T> task) {
    return detail::taskAsAwaitable(std::move(task));
}

}  // namespace ruvia
