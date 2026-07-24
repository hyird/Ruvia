#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "ruvia/web/detail/app/AppAccess.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

// Invokes the per-request access-log callback for a terminal outcome whose final
// response status was committed. A no-op (single null check) when unset, so it
// stays off the cost ledger of servers that do not observe. `status` comes from
// the committed protocol plan; duration is measured from `start`.
inline void recordHttpAccess(const AccessLogSink& accessLog, const HttpRequest& request, std::string_view remoteAddress, HttpStatusCode status, std::chrono::steady_clock::time_point start) noexcept {
    if (!accessLog.callback) {
        return;
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    const AccessLogRecord record = AccessLogRecordAccess::make(request, remoteAddress, status, micros < 0 ? 0 : static_cast<std::uint64_t>(micros));
    accessLog.invoke(record);
}

}  // namespace ruvia::detail
