#pragma once

#include "ruvia/app/App.h"

namespace ruvia::detail {

struct AccessLogRecordAccess final {
    [[nodiscard]] static constexpr AccessLogRecord make(
        HttpMethod method,
        std::string_view path,
        std::string_view remoteAddress,
        std::uint16_t status,
        std::uint64_t durationMicros,
        bool http2) noexcept {
        return AccessLogRecord(method, path, remoteAddress, status, durationMicros, http2);
    }
};

}  // namespace ruvia::detail
