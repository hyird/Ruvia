#pragma once

#include <utility>

#include "ruvia/core/AsioTask.h"

namespace ruvia {

// Adapt a completion-based Asio operation for use with a Ruvia coroutine.
// The awaiter owns the completion state until the operation finishes.
template <typename Result = void, typename Initiate>
[[nodiscard]] auto asyncAsio(Initiate&& initiate) {
    return detail::asyncAsio<Result>(std::forward<Initiate>(initiate));
}

}  // namespace ruvia
