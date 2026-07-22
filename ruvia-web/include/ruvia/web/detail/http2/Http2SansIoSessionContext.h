#pragma once

#include <cstddef>

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerWorkerState.h"

// What an HTTP/2 session needs from the server that accepted the connection, and
// the inactivity phase it reports back to the connection scanner. Both are pure
// values: a session driver takes the context by value and never reaches past it
// into the server.

namespace ruvia::detail {

class Http2SansIoSessionContext final {
public:
    Http2SansIoSessionContext(
        ContextServices services,
        const HttpServerOptions& options,
        ConnectionScanner::Entry& scannerEntry,
        const HttpServerWorkerState& workerState) noexcept
        : services_(services),
          options_(&options),
          scannerEntry_(&scannerEntry),
          workerState_(&workerState) {}

    [[nodiscard]] const HttpServerOptions& options() const noexcept {
        return *options_;
    }

    [[nodiscard]] ConnectionScanner::Entry& scannerEntry() const noexcept {
        return *scannerEntry_;
    }

    [[nodiscard]] bool workerRunning() const noexcept {
        return httpServerWorkerRunning(*workerState_);
    }

    [[nodiscard]] const ContextServices& services() const noexcept {
        return services_;
    }

private:
    ContextServices services_;
    const HttpServerOptions* options_;
    ConnectionScanner::Entry* scannerEntry_;
    const HttpServerWorkerState* workerState_;
};

[[nodiscard]] inline ConnectionScanner::Phase http2SansIoInactivityPhase(
    bool headerBlockInProgress,
    std::size_t activeRuntimeCount) noexcept {
    if (headerBlockInProgress) {
        return ConnectionScanner::Phase::kReadingInitial;
    }
    return activeRuntimeCount == 0
        ? ConnectionScanner::Phase::kIdle
        : ConnectionScanner::Phase::kReadingPayload;
}

}  // namespace ruvia::detail
