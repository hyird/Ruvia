#pragma once

#include "ruvia/web/HttpServerOptions.h"

namespace ruvia::detail {

struct AccessLogRecordAccess final {
    [[nodiscard]] static constexpr AccessLogRecord make(
        const HttpRequest& request,
        std::string_view remoteAddress,
        std::uint16_t status,
        std::uint64_t durationMicros) noexcept {
        return AccessLogRecord(
            request,
            remoteAddress,
            status,
            durationMicros);
    }
};

}  // namespace ruvia::detail
