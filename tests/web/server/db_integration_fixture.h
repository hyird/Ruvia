#pragma once

#include "test_harness.h"

// Shared by the two live-driver tests. Both are skipped unless their
// RUVIA_RUN_*_INTEGRATION variable says otherwise, both read connection details
// from the environment, and both assert with the same two helpers -- keeping one
// copy means a change to how they are configured cannot reach one driver and
// miss the other.

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ruvia::testing {

[[nodiscard]] inline std::string_view dbEnvironment(
    const char* name, std::string_view fallback) noexcept {
    const auto* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string_view(value) : fallback;
}

[[nodiscard]] inline bool dbIntegrationRequested(const char* name) noexcept {
    const auto* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] inline std::uint16_t dbEnvironmentPort(const char* name, std::string_view fallback) {
    const auto text = dbEnvironment(name, fallback);
    unsigned parsed = 0;
    for (const auto character : text) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument(std::string(name).append(" must be numeric"));
        }
        parsed = parsed * 10U + static_cast<unsigned>(character - '0');
        if (parsed > 65535) {
            break;
        }
    }
    if (parsed == 0 || parsed > 65535) {
        throw std::invalid_argument(std::string(name).append(" is outside the valid range"));
    }
    return static_cast<std::uint16_t>(parsed);
}

inline void dbRequire(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

}  // namespace ruvia::testing

