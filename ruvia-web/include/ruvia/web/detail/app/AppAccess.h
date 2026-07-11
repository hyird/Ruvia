#pragma once

#include "ruvia/web/HttpServerOptions.h"

namespace ruvia::detail {

struct AccessLogRecordAccess final {
    [[nodiscard]] static constexpr AccessLogRecord make(
        std::string_view method,
        HttpKnownMethod knownMethod,
        std::string_view path,
        std::string_view remoteAddress,
        std::uint16_t status,
        std::uint64_t durationMicros,
        bool http2) noexcept {
        return AccessLogRecord(
            method, knownMethod, path, remoteAddress, status, durationMicros, http2);
    }
};

}  // namespace ruvia::detail
