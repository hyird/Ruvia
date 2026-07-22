#pragma once

#include <stdexcept>

#include "ruvia/web/detail/ConfigValidation.h"

namespace ruvia::detail {

inline void ensureAppNotRunning(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}

}  // namespace ruvia::detail
