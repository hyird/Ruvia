#pragma once

#include <stdexcept>

#include "ruvia/core/config_validation.h"

namespace ruvia::detail {

inline void ensure_app_not_running(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}

}  // namespace ruvia::detail
