#pragma once

#include <cstddef>
#include <utility>

#include "ruvia/core/ConnectionScanner.h"

#include "context/ContextServices.h"
#include "server/HttpServerOptions.h"
#include "server/HttpServerWorkerState.h"

// What an HTTP/2 session needs from the server that accepted the connection.
// ContextServices is copied per session; the remaining dependencies are stable
// server-owned borrows that cannot be absent or rebound.

namespace ruvia::detail {

class Http2SansIoSessionContext final {
public:
    Http2SansIoSessionContext(ContextServices services, const HttpServerOptions& options,
        ruvia::ConnectionScanner::Entry& scannerEntry, const HttpServerWorkerState& workerState)
        : services_(std::move(services)),
          options_(options),
          scannerEntry_(scannerEntry),
          workerState_(workerState) {}

    [[nodiscard]] const HttpServerOptions& options() const noexcept {
        return options_;
    }

    [[nodiscard]] ruvia::ConnectionScanner::Entry& scannerEntry() const noexcept {
        return scannerEntry_;
    }

    [[nodiscard]] bool workerRunning() const noexcept {
        return httpServerWorkerRunning(workerState_);
    }

    [[nodiscard]] const ContextServices& services() const noexcept {
        return services_;
    }

private:
    ContextServices services_;
    const HttpServerOptions& options_;
    ruvia::ConnectionScanner::Entry& scannerEntry_;
    const HttpServerWorkerState& workerState_;
};

[[nodiscard]] inline ruvia::ConnectionScanner::Phase http2SansIoInactivityPhase(
    bool headerBlockInProgress, std::size_t activeRuntimeCount,
    bool webSocketTunnelActive) noexcept {
    if (headerBlockInProgress) {
        return ruvia::ConnectionScanner::Phase::kReadingInitial;
    }
    if (webSocketTunnelActive) {
        // A successful RFC 8441 tunnel is long-lived, not a stalled request body.
        // Keep the header timeout above so another stream's incomplete header block
        // cannot hold the connection open indefinitely.
        return ruvia::ConnectionScanner::Phase::kLongLived;
    }
    return activeRuntimeCount == 0 ? ruvia::ConnectionScanner::Phase::kIdle
                                   : ruvia::ConnectionScanner::Phase::kReadingPayload;
}

}  // namespace ruvia::detail
