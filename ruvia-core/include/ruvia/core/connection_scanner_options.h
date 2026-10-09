#pragma once

#include <chrono>
#include <optional>

namespace ruvia {

// Worker-owned connection activity tracking options shared by protocol runtimes.
struct connection_scanner_options final {
    std::chrono::milliseconds scan_interval_{std::chrono::seconds(1)};
    // Inactivity timeouts measured from the connection's last successful I/O.
    // Absence disables the corresponding phase timeout.
    std::optional<std::chrono::milliseconds> idle_timeout_{};
    std::optional<std::chrono::milliseconds> initial_read_timeout_{};
    std::optional<std::chrono::milliseconds> payload_read_timeout_{};
    std::optional<std::chrono::milliseconds> write_timeout_{};
    // Absolute phase deadlines, independent of successful I/O progress.
    std::optional<std::chrono::milliseconds> initial_read_completion_timeout_{};
    std::optional<std::chrono::milliseconds> payload_read_completion_timeout_{};
};

}  // namespace ruvia
