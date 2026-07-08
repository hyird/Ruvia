#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "app/AppAccess.h"
#include "ruvia/http/HttpServerOptions.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

// Invokes the per-request access-log callback once a response has completed.
// A no-op (single null check) when unset, so it stays off the cost ledger of
// servers that do not observe. status is the response status (200 for streams);
// duration is measured from `start`.
inline void recordHttpAccess(
    const HttpServerOptions::AccessLog& accessLog,
    const HttpRequest& request,
    std::string_view remoteAddress,
    std::uint16_t status,
    std::chrono::steady_clock::time_point start,
    bool http2) noexcept {
    if (accessLog.callback == nullptr) {
        return;
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    const AccessLogRecord record = AccessLogRecordAccess::make(
        request.method(),
        request.path(),
        remoteAddress,
        status,
        micros < 0 ? 0 : static_cast<std::uint64_t>(micros),
        http2);
    accessLog.callback(accessLog.user, record);
}

}  // namespace ruvia::detail
