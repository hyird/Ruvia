#pragma once

#include <utility>

#include "ruvia/core/asio_task.h"

namespace ruvia {

// Adapt a completion-based Asio operation for use with a Ruvia coroutine.
// The awaiter owns the completion state until the operation finishes.
template <typename result_type = void, typename initiate_type>
[[nodiscard]] auto async_asio(initiate_type&& initiate) {
    return detail::async_asio<result_type>(std::forward<initiate_type>(initiate));
}

}  // namespace ruvia
