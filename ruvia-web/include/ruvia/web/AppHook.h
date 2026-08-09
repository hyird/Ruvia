#pragma once

#include <cstddef>

#include "ruvia/web/detail/Callback.h"

namespace ruvia {

// A self-contained startup/shutdown callback. Called with no arguments when
// the app starts (onStart) or stops (onStop). A self-contained callable is
// owned by the hook value; references captured by that callable must still
// outlive the registered hook. Invoking an empty hook is a programming error
// and throws std::logic_error.
using AppHook = detail::Callback<void()>;

static_assert(sizeof(AppHook) == 5 * sizeof(void*));

}  // namespace ruvia
