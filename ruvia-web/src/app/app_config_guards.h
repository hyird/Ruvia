#pragma once

#include <stdexcept>

namespace ruvia::detail {

inline void ensure_app_not_running(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}

}  // namespace ruvia::detail
