#pragma once

#include <chrono>
#include <stdexcept>

namespace ruvia::detail {

inline void ensureAppNotRunning(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}

template <typename Rep, typename Period>
void ensureNonNegativeDuration(std::chrono::duration<Rep, Period> value, const char* message) {
    if (value.count() < 0) {
        throw std::invalid_argument(message);
    }
}

}  // namespace ruvia::detail
