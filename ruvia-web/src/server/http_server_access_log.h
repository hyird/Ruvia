#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "ruvia/http/http_request.h"

#include "app/app_access.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

// Invokes the per-request access-log callback for a terminal outcome whose final
// response status was committed. A no-op (single null check) when unset, so it
// stays off the cost ledger of servers that do not observe. `status` comes from
// the committed protocol plan; duration is measured from `start`.
inline void record_http_access(const access_log_sink& access_log, const http_request& request,
    std::string_view remote_address, http_status_code status,
    std::chrono::steady_clock::time_point start) noexcept {
    if (!access_log.callback_) {
        return;
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start)
                            .count();
    const access_log_record record = access_log_record_access::make(
        request, remote_address, status, micros < 0 ? 0 : static_cast<std::uint64_t>(micros));
    access_log.invoke(record);
}

}  // namespace ruvia::detail
