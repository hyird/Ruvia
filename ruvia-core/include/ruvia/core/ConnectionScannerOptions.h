#pragma once

#include <chrono>
#include <optional>

namespace ruvia {

// Worker-owned connection activity tracking options shared by protocol runtimes.
struct ConnectionScannerOptions final {
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    // Inactivity timeouts measured from the connection's last successful I/O.
    // Absence disables the corresponding phase timeout.
    std::optional<std::chrono::milliseconds> idleTimeout{};
    std::optional<std::chrono::milliseconds> initialReadTimeout{};
    std::optional<std::chrono::milliseconds> payloadReadTimeout{};
    std::optional<std::chrono::milliseconds> writeTimeout{};
};

}  // namespace ruvia
