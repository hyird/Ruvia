#pragma once

#include "ruvia/web/ServerConfig.h"

namespace ruvia::detail {

struct AccessLogRecordAccess final {
    [[nodiscard]] static constexpr AccessLogRecord make(const HttpRequest& request,
        std::string_view remoteAddress, HttpStatusCode status,
        std::uint64_t durationMicros) noexcept {
        return AccessLogRecord(request, remoteAddress, status, durationMicros);
    }
};

}  // namespace ruvia::detail
