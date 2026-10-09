#pragma once

#include <cstddef>

#include "ruvia/web/detail/callback.h"

namespace ruvia {

// A self-contained startup/shutdown callback, invoked on the application::run() caller.
// on_start runs after worker initialization, before TCP/QUIC admission. on_stop
// runs after admission closes, before runtime join (including start-hook failure
// or cancellation). A self-contained callable is owned by the hook value;
// references captured by that callable must still
// outlive the registered hook. Invoking an empty hook is a programming error
// and throws std::logic_error.
using app_hook_type = detail::callback<void()>;

static_assert(sizeof(app_hook_type) == 5 * sizeof(void*));

}  // namespace ruvia
